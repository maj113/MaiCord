#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/app/account_module_version.hpp"
#include "maicord/app/account_profile.hpp"

#if defined(MAICORD_PERF_PROBE)
#include "../../tools/perf/perf_shared.hpp"
#endif

namespace maicord {
namespace {

// MaiCordPerfProbe loads the synthetic account module beside it instead.
#if defined(MAICORD_PERF_PROBE)
constexpr const wchar_t (&kHostModuleFileName)[sizeof(perf::kPerfAccountModuleFileName) / sizeof(wchar_t)] =
    perf::kPerfAccountModuleFileName;
#else
constexpr const wchar_t (&kHostModuleFileName)[sizeof(kAccountModuleFileNameWide) / sizeof(wchar_t)] =
    kAccountModuleFileNameWide;
#endif

AccountHostResult failure(AccountHostError error, u32 native_error = 0) noexcept {
    return AccountHostResult{native_error, error};
}

// No module call was made. login_refused also reports a failed phase and a native error.
AccountLoginStatus login_invalid_input() noexcept {
    AccountLoginStatus status{};
    status.error = AccountLoginError::invalid_input;
    return status;
}

AccountLoginStatus login_refused() noexcept {
    AccountLoginStatus status = login_invalid_input();
    status.native_error = ERROR_INVALID_PARAMETER;
    status.phase = AccountLoginPhase::failed;
    return status;
}

bool host_empty(const AccountHost& host) noexcept {
    return !host.module && !host.instance && !host.login_instance;
}

} // namespace

bool account_host_api_valid(const AccountModuleApi& api) noexcept {
    return api.version == kAccountModuleApiVersion && api.bytes == sizeof(AccountModuleApi) && api.start &&
           api.pump && api.notification_handles && api.gateway_source && api.gateway_pop &&
           api.guild_subscribe && api.gateway_refresh && api.private_conversation &&
           api.attachment_upload && api.upload && api.history_submit && api.message_submit &&
           api.message_delete && api.message_edit && api.message_reaction && api.reaction_users &&
           api.message_typing && api.media_url_refresh && api.rest_take && api.media_request &&
           api.media_guild_icon_request && api.media_attachment_request && api.media_advance &&
           api.media_take && api.runtime_bytes && api.video_select && api.video_control &&
           api.video_status && api.video_take && api.video_acknowledge && api.message_search && api.stop &&
           api.login_start && api.login_submit_password && api.login_submit_mfa && api.login_send_sms &&
           api.login_pump && api.login_copy_captcha_secret && api.login_submit_captcha &&
           api.login_start_remote_auth && api.login_copy_remote_auth_qr &&
           api.login_copy_remote_auth_user && api.login_cancel_remote_auth &&
           api.login_copy_webauthn_challenge && api.login_notification_handle && api.login_stop &&
           api.application_command && api.set_rest_trace && api.login_submit_verification &&
           api.relationships && api.message_ack && api.login_submit_recovery && api.login_start_reset &&
           api.login_submit_reset && api.login_submit_phone_verify && api.login_submit_phone_resend &&
           api.login_take_phone_reset_token && api.login_start_browser && api.login_copy_browser_url &&
           api.self_status_snapshot && api.self_status_submit && api.self_status_complete &&
           api.media_catalog && api.catalog_preview_request && api.gif_favorites && api.channel_overview &&
           api.guild_members_request && api.pinned_messages && api.message_forward && api.download &&
           api.thread_action && api.thread_browser && api.thread_creation && api.forum_post &&
           api.media_local_preview && api.logout && api.bulk_message_ack && api.poll && api.guild_admin &&
           api.voice_request && api.voice_snapshot && api.login_start_captcha_bridge &&
           api.login_captcha_bridge_status && api.login_copy_captcha_bridge_url && api.login_cancel_captcha_bridge &&
           api.voice_user_audio && api.voice_audio_configure && api.voice_audio_status && api.guild_invite &&
           api.emoji_preferences && api.action_captcha && api.account_settings && api.poll_create &&
           api.private_channel && api.custom_status_snapshot && api.custom_status_submit && api.guild_settings &&
           api.mentions && api.guild_assets && api.voice_video_snapshot && api.voice_video_wanted &&
           api.voice_video_device && api.voice_video_frame && api.stream_watch && api.stream_snapshot &&
           api.stream_volume && api.stream_preview && api.stage && api.message_interaction && api.user_social &&
           api.guild_community && api.local_presence && api.activity_sharing &&
           api.soundboard && api.soundboard_audio && api.voice_message && api.profile_edit &&
           api.profile_image && api.decoders.message_create && api.decoders.message_create_mentions &&
           api.decoders.message_create_users && api.decoders.message_create_rich &&
           api.decoders.message_snapshot_core && api.decoders.message_snapshot_extract &&
           api.decoders.message_poll && api.decoders.channel_create && api.decoders.private_channel &&
           api.cdn_urls.parse_signed_attachment_url && api.cdn_urls.parse_media_url &&
           api.cdn_urls.catalog_provider_target_valid && api.cdn_urls.sticker_json_target_valid &&
           api.cdn_urls.soundboard_target_valid && api.cdn_urls.parse_catalog_preview_url &&
           api.cdn_urls.catalog_favorite_format && api.cdn_urls.media_url_log_fields &&
           api.cdn_urls.write_media_proxy_target && api.cdn_urls.write_custom_emoji_cdn_target &&
           api.cdn_urls.cdn_guild_icon_variant && api.cdn_urls.write_guild_icon_cdn_target &&
           api.cdn_urls.write_user_avatar_cdn_target && api.cdn_urls.write_channel_icon_cdn_target &&
           api.cdn_urls.write_sticker_cdn_target && api.cdn_urls.cdn_host_name && api.guild_moderation &&
           api.guild_members_query_cancel && api.channel_info;
}

AccountHostResult account_host_module_path(Slice<wchar_t> output) noexcept {
    if (!output.data || output.count == 0) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, kAccountProfileMaximumPathCharacters);
    if (length == 0 || length >= kAccountProfileMaximumPathCharacters) {
        return failure(AccountHostError::module_path,
                       length == 0 ? thread_last_error() : ERROR_INSUFFICIENT_BUFFER);
    }
    u32 separator = length;
    while (separator != 0 && path[separator - 1] != L'\\' && path[separator - 1] != L'/') {
        --separator;
    }
    constexpr u32 file_name_length =
        static_cast<u32>(sizeof(kHostModuleFileName) / sizeof(kHostModuleFileName[0]) - 1);
    const u32 required = separator + file_name_length + 1;
    if (required > output.count) {
        SecureZeroMemory(path, sizeof(path));
        return failure(AccountHostError::module_path, ERROR_INSUFFICIENT_BUFFER);
    }
    copy_bytes(path + separator, kHostModuleFileName,
               file_name_length * static_cast<u32>(sizeof(wchar_t)));
    path[separator + file_name_length] = 0;
    copy_bytes(output.data, path, required * static_cast<u32>(sizeof(wchar_t)));
    SecureZeroMemory(path, sizeof(path));
    return AccountHostResult{};
}

AccountHostResult account_host_load(AccountHost* host) noexcept {
    if (!host) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    if (host->module) {
        return account_host_api_valid(host->api)
                   ? AccountHostResult{}
                   : failure(AccountHostError::api_mismatch, ERROR_INVALID_STATE);
    }
    if (!host_empty(*host)) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_STATE);
    }
    wchar_t module_path[kAccountProfileMaximumPathCharacters]{};
    const AccountHostResult path =
        account_host_module_path(Slice<wchar_t>{module_path, kAccountProfileMaximumPathCharacters});
    if (!path.ok()) {
        return path;
    }
    HMODULE module = LoadLibraryExW(module_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    SecureZeroMemory(module_path, sizeof(module_path));
    if (!module) {
        return failure(AccountHostError::module_load, thread_last_error());
    }
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    if (!get_api) {
        const u32 native_error = thread_last_error();
        FreeLibrary(module);
        return failure(AccountHostError::module_symbol, native_error);
    }
    AccountModuleApi api{};
    if (!get_api(kAccountModuleApiVersion, &api) || !account_host_api_valid(api)) {
        FreeLibrary(module);
        return failure(AccountHostError::api_mismatch, ERROR_REVISION_MISMATCH);
    }
    AccountHost loaded{};
    loaded.module = module;
    loaded.api = api;
    loaded.last_phase = 0xff;
    loaded.last_ready = ~0u;
    *host = loaded;
    return AccountHostResult{};
}

AccountHostResult account_host_release(AccountHost* host) noexcept {
    if (!host) {
        return failure(AccountHostError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    if (host->instance || host->login_instance) {
        return failure(AccountHostError::active, ERROR_BUSY);
    }
    if (host->module && !FreeLibrary(host->module)) {
        return failure(AccountHostError::module_release, thread_last_error());
    }
    *host = AccountHost{};
    return AccountHostResult{};
}

u32 account_host_observe(AccountHost* host, const AccountModuleStatus& status) noexcept {
    if (!host) {
        return 0;
    }
    u32 observations = 0;
    if (host->last_phase != status.coordinator_phase ||
        host->status.required_action != status.required_action) {
        host->last_phase = status.coordinator_phase;
        observations |= account_host_observation_phase;
    }
    if (host->last_ready != status.gateway_ready) {
        host->last_ready = status.gateway_ready;
        observations |= account_host_observation_ready;
    }
    if (host->last_session_saves != status.session_saves) {
        host->last_session_saves = status.session_saves;
        observations |= account_host_observation_session_saves;
    }
    if (host->last_session_save_failures != status.session_save_failures) {
        host->last_session_save_failures = status.session_save_failures;
        observations |= account_host_observation_session_save_failures;
    }
    host->status = status;
    return observations;
}

AccountModuleStartResult account_host_start(AccountHost* host, const wchar_t* profile_path,
                                            const wchar_t* session_path, u64 monotonic_now_ms,
                                            u64 unix_now_ms) noexcept {
    if (!host || !host->module || host->instance || !profile_path || !session_path ||
        !account_host_api_valid(host->api)) {
        AccountModuleStartResult invalid{};
        invalid.native_error = ERROR_INVALID_PARAMETER;
        invalid.error = AccountModuleError::invalid_input;
        return invalid;
    }
    AccountModuleStartResult started =
        host->api.start(profile_path, session_path, monotonic_now_ms, unix_now_ms);
    if (started.ok()) {
        host->instance = started.instance;
    }
    return started;
}

AccountLoginStatus account_host_login_start(AccountHost* host, const wchar_t* profile_path) noexcept {
    if (!host || !host->module || host->login_instance || host->stop_failed || !profile_path ||
        !host->api.login_start) {
        return login_refused();
    }
    host->login_status = host->api.login_start(&host->login_instance, profile_path);
    return host->login_status;
}

AccountLoginStatus account_host_login_start_reset(AccountHost* host, const wchar_t* profile_path) noexcept {
    if (!host || !host->module || host->login_instance || host->stop_failed || !profile_path ||
        !host->api.login_start_reset) {
        return login_refused();
    }
    host->login_status = host->api.login_start_reset(&host->login_instance, profile_path);
    return host->login_status;
}

AccountLoginStatus account_host_login_submit_reset(AccountHost* host, const SecureBuffer* link_token,
                                                   const SecureBuffer* password) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_reset || !password) {
        return login_refused();
    }
    host->login_status = host->api.login_submit_reset(host->login_instance, link_token, password);
    return host->login_status;
}

AccountModuleStatus account_host_pump(AccountHost* host, u64 monotonic_now_ms, u64 unix_now_ms,
                                      u32* observations) noexcept {
    if (observations) {
        *observations = 0;
    }
    if (!host || !host->module || !host->instance || host->stop_failed || !host->api.pump) {
        AccountModuleStatus invalid{};
        invalid.native_error = ERROR_INVALID_PARAMETER;
        invalid.error = AccountModuleError::invalid_input;
        invalid.failed = true;
        return invalid;
    }
    const AccountModuleStatus status = host->api.pump(host->instance, monotonic_now_ms, unix_now_ms);
    const u32 changed = account_host_observe(host, status);
    if (observations) {
        *observations = changed;
    }
    return status;
}

AccountLoginStatus account_host_login_submit_verification(AccountHost* host,
                                                          const SecureBuffer* link_token) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_verification || !link_token) {
        return login_refused();
    }
    host->login_status = host->api.login_submit_verification(host->login_instance, link_token);
    return host->login_status;
}

AccountLoginStatus account_host_login_submit_recovery(AccountHost* host,
                                                      const SecureBuffer* login) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_recovery || !login) {
        return login_invalid_input();
    }
    host->login_status = host->api.login_submit_recovery(host->login_instance, login);
    return host->login_status;
}

AccountLoginStatus account_host_login_pump(AccountHost* host) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed || !host->api.login_pump)
        return login_refused();
    host->login_status = host->api.login_pump(host->login_instance);
    return host->login_status;
}

AccountLoginStatus account_host_login_submit_phone_verify(AccountHost* host,
                                                          const SecureBuffer* code) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_phone_verify || !code) {
        return login_invalid_input();
    }
    host->login_status = host->api.login_submit_phone_verify(host->login_instance, code);
    return host->login_status;
}
AccountLoginStatus account_host_login_submit_phone_resend(AccountHost* host) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_submit_phone_resend) {
        return login_invalid_input();
    }
    host->login_status = host->api.login_submit_phone_resend(host->login_instance);
    return host->login_status;
}
SecureBufferResult account_host_login_take_phone_reset_token(AccountHost* host, u64 expected_generation,
                                                             SecureBuffer* output) noexcept {
    if (!host || !host->module || !host->login_instance || host->stop_failed ||
        !host->api.login_take_phone_reset_token)
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    const auto result =
        host->api.login_take_phone_reset_token(host->login_instance, expected_generation, output);
    if (result.ok()) {
        host->login_status.phase = AccountLoginPhase::phone_token_taken;
        host->login_status.generation = expected_generation == ~0ull ? 1 : expected_generation + 1;
        host->login_status.request_id = 0;
    }
    return result;
}

AccountLoginStatus account_host_login_stop(AccountHost* host, u32 timeout_ms) noexcept {
    if (!host || !host->module || !host->login_instance || timeout_ms == 0 || !host->api.login_stop)
        return login_refused();
    host->login_status = host->api.login_stop(&host->login_instance, timeout_ms);
    if (host->login_instance) {
        host->stop_failed = true;
    } else if (!host->instance) {
        host->stop_failed = false;
    }
    return host->login_status;
}

AccountHostStopResult account_host_stop(AccountHost* host, u32 timeout_ms, u64 monotonic_now_ms,
                                        u64 unix_now_ms) noexcept {
    AccountHostStopResult result{};
    if (!host || timeout_ms == 0) {
        result.native_error = ERROR_INVALID_PARAMETER;
        result.error = AccountHostError::invalid_input;
        return result;
    }
    if (!host->module) {
        if (!host_empty(*host)) {
            result.native_error = ERROR_INVALID_STATE;
            result.error = AccountHostError::invalid_input;
        }
        return result;
    }
    if (host->login_instance) {
        result.login_status = account_host_login_stop(host, timeout_ms);
        if (host->login_instance) {
            host->stop_failed = true;
            result.native_error = result.login_status.native_error;
            result.error = AccountHostError::login_stop;
            return result;
        }
    }
    if (host->instance) {
        result.account_status = host->api.stop(&host->instance, timeout_ms, monotonic_now_ms, unix_now_ms);
        result.observations = account_host_observe(host, result.account_status);
        if (host->instance) {
            host->stop_failed = true;
            result.native_error = result.account_status.native_error;
            result.error = AccountHostError::account_stop;
            return result;
        }
    } else {
        result.account_status = host->status;
    }
    const AccountHostResult released = account_host_release(host);
    if (!released.ok()) {
        result.native_error = released.native_error;
        result.error = released.error;
    }
    return result;
}

u32 account_host_notification_handles(const AccountHost* host, HANDLE* output, u32 capacity) noexcept {
    if (!host || !output || capacity == 0 || capacity > kAccountHostNotificationHandleCapacity ||
        !host->module) {
        return 0;
    }
    HANDLE staged[kAccountHostNotificationHandleCapacity]{};
    u32 count = 0;
    if (host->instance && !host->stop_failed) {
        count = host->api.notification_handles(host->instance, staged, capacity);
        if (count > capacity) {
            return 0;
        }
    }
    if (host->login_instance && !host->stop_failed && count < capacity) {
        const HANDLE login = host->api.login_notification_handle(host->login_instance);
        bool duplicate = false;
        for (u32 index = 0; index < count; ++index) {
            duplicate = duplicate || staged[index] == login;
        }
        if (login && !duplicate) {
            staged[count++] = login;
        }
    }
    copy_bytes(output, staged, count * static_cast<u32>(sizeof(HANDLE)));
    return count;
}

u32 account_host_wait_timeout(const AccountHost* host, u64 now_ms) noexcept {
    if (!host || !host->instance || host->stop_failed || host->status.next_wake_at_ms == 0) {
        return INFINITE;
    }
    if (host->status.next_wake_at_ms <= now_ms) {
        return 0;
    }
    const u64 remaining = host->status.next_wake_at_ms - now_ms;
    return remaining >= INFINITE ? INFINITE - 1 : static_cast<u32>(remaining);
}

bool account_host_wake_due(const AccountHost* host, u64 now_ms) noexcept {
    return host && host->instance && !host->stop_failed && host->status.next_wake_at_ms != 0 &&
           now_ms >= host->status.next_wake_at_ms;
}

u32 account_host_runtime_bytes() noexcept {
    return sizeof(AccountHost);
}

} // namespace maicord
