// The linked account module's entries called without an account owner: each refuses before creating an account, login,
// worker or transport. The tests that fake the module link tests/support/account_module_fake.cpp instead.
#include "../../support/test_output.hpp"

#include "maicord/app/account/account_host.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/app/gateway/state_recovery.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/account_upload.hpp"
#include "maicord/app/net/rest_client.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/protocol/messages/history.hpp"
#include "maicord/protocol/messages/poll.hpp"
#include "maicord/protocol/auth/user_auth.hpp"
#include "maicord/protocol/user/private_conversation.hpp"
#include "maicord/protocol/user/relationships.hpp"

namespace maicord {

void test_account_module_dormant(void (*check)(bool, const char*) noexcept) noexcept;

namespace {

u32 failures;

void expect(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

void test_account_module_contract() noexcept {
    PollRequest dormant_poll{};
    dormant_poll.channel_id = 1;
    dormant_poll.message_id = 2;
    dormant_poll.operation = PollOperation::refresh;
    expect(account_module_poll(nullptr, &dormant_poll, 1).error == RestClientSubmitError::invalid_input &&
               account_module_poll(nullptr, nullptr, 1).error == RestClientSubmitError::invalid_input,
           "account_module_poll_dormant_submit_and_exact_cancel_without_owner");
    expect(account_module_media_local_preview(nullptr, AccountLocalPreviewOperation::request, nullptr, 1, 1).error ==
               AccountMediaError::invalid_input,
           "account_module_local_preview_dormant_contract");
    AccountMediaLocalPreviewRequest poster_receipt{};
    poster_receipt.lease_serial = 17;
    expect(account_module_media_local_preview(nullptr, AccountLocalPreviewOperation::uploaded, &poster_receipt, 0, 0)
                   .error == AccountMediaError::invalid_input &&
               account_module_media_local_preview(nullptr, AccountLocalPreviewOperation::presented, nullptr, 0, 0)
                   .error == AccountMediaError::invalid_input,
           "account_module_poster_operations_require_an_owner");
    // Invalid operation shapes must fail before any owner or worker access.
    // An inaccessible reservation makes accidental access fail deterministically.
    void* guarded_owner = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    expect(guarded_owner != nullptr, "account_module_poster_shape_guard_reservation");
    if (guarded_owner) {
        struct InvalidPosterShape {
            AccountLocalPreviewOperation operation;
            bool request;
            u64 now_ms, now_seconds;
        };
        constexpr InvalidPosterShape invalid_shapes[] = {
            {AccountLocalPreviewOperation::uploaded, false, 0, 0},
            {AccountLocalPreviewOperation::uploaded, true, 1, 0},
            {AccountLocalPreviewOperation::uploaded, true, 0, 1},
            {AccountLocalPreviewOperation::presented, true, 0, 0},
            {AccountLocalPreviewOperation::presented, false, 1, 0},
            {AccountLocalPreviewOperation::presented, false, 0, 1},
            {static_cast<AccountLocalPreviewOperation>(255), false, 0, 0},
        };
        for (const auto& shape : invalid_shapes) {
            const auto result = account_module_media_local_preview(guarded_owner, shape.operation,
                shape.request ? &poster_receipt : nullptr, shape.now_ms, shape.now_seconds);
            expect(result.error == AccountMediaError::invalid_input,
                   "account_module_rejects_poster_operation_shape_before_owner_access");
        }
    }
    if (guarded_owner)
        VirtualFree(guarded_owner, 0, MEM_RELEASE);
    expect(account_module_private_conversation(nullptr, PrivateConversationOperation::open, 2, 1).error ==
               RestClientSubmitError::invalid_input,
           "account_module_private_conversation_contract");
    expect(account_module_login_submit_reset(nullptr, nullptr, nullptr).error == AccountLoginError::invalid_input,
           "account_module_reset_rejects_missing_owner_and_locked_password");
    expect(account_module_login_submit_phone_verify(nullptr, nullptr).error == AccountLoginError::invalid_input &&
               account_module_login_submit_phone_resend(nullptr).error == AccountLoginError::invalid_input &&
               account_module_login_take_phone_reset_token(nullptr, 0, nullptr).error ==
                   SecureBufferError::invalid_input,
           "account_module_phone_entries_reject_missing_owner_without_work");
    expect(account_module_login_submit_recovery(nullptr, nullptr).error == AccountLoginError::invalid_input,
           "account_module_recovery_rejects_missing_owner_and_locked_identifier");
    expect(account_module_attachment_upload(nullptr, 7, 0, utf8_literal("a.txt"), 1, 1).error ==
               RestClientSubmitError::invalid_input,
           "account_module_attachment_upload_contract");
    expect(account_module_upload(nullptr, AccountUploadOperation::status, 1, nullptr).submit_error ==
               HttpsFileUploadError::invalid_input,
           "account_module_upload_transport_contract");
    const AccountModuleStartResult missing = account_module_start(
        L"Z:\\MaiCord-definitely-missing.profile", L"Z:\\MaiCord-definitely-missing.session", 1, 1);
    expect(!missing.ok() && missing.instance == nullptr && missing.error == AccountModuleError::profile &&
               missing.native_error != 0,
           "account_module_missing_profile_is_dormant");
    {
        GatewayClientEvent event{};
        u8 payload[1]{};
        MessageHistoryQuery query{0, 1, MessageHistoryPosition::latest};
        RestClientResponse response{};
        AccountMediaFrame media_frame{};
        AccountMediaAttachmentRequest attachment_request{};
        void* empty_instance = nullptr;
        expect(account_module_pump(nullptr, 1, 1).error == AccountModuleError::invalid_input &&
                   account_module_notification_handles(nullptr, nullptr, 0) == 0 &&
                   account_module_gateway_source(nullptr) == nullptr &&
                   account_module_gateway_pop(nullptr, &event, Slice<u8>{payload, 1}).error ==
                       GatewayClientPopError::invalid_input &&
                   !account_module_guild_subscribe(nullptr, nullptr) &&
                   account_module_gateway_refresh(nullptr) == GatewayStateRecoveryResult::unavailable &&
                   account_module_history_submit(nullptr, 1, &query, 1).error ==
                       RestClientSubmitError::invalid_input &&
                   account_module_message_typing(nullptr, 1, 1).error == RestClientSubmitError::invalid_input &&
                   account_module_rest_take(nullptr, &response, Slice<u8>{payload, 1}).error ==
                       RestClientTakeError::invalid_input &&
                   account_module_media_request(nullptr, media_asset_sticker(1, 1), 1, 1).error ==
                       AccountMediaError::invalid_input &&
                   account_module_media_attachment_request(nullptr, &attachment_request, 1, 1).error ==
                       AccountMediaError::invalid_input &&
                   account_module_media_advance(nullptr, media_asset_sticker(1, 2), 1, 1, 1).error ==
                       AccountMediaError::invalid_input &&
                   !account_module_media_take(nullptr, &media_frame) &&
                   account_module_stop(&empty_instance, 1, 1, 1).error == AccountModuleError::invalid_input &&
                   account_module_login_start(nullptr, L"Z:\\invalid.profile").error ==
                       AccountLoginError::invalid_input &&
                   account_module_login_submit_password(nullptr, nullptr, nullptr).error ==
                       AccountLoginError::invalid_input &&
                   account_module_login_submit_mfa(nullptr, nullptr, UserAuthMfaType::totp).error ==
                       AccountLoginError::invalid_input &&
                   account_module_login_send_sms(nullptr).error == AccountLoginError::invalid_input &&
                   account_module_login_start_remote_auth(nullptr).error == AccountLoginError::invalid_input &&
                   !account_module_login_copy_remote_auth_qr(nullptr, Slice<char>{}, nullptr) &&
                   !account_module_login_copy_remote_auth_user(nullptr, Slice<char>{}, nullptr) &&
                   account_module_login_cancel_remote_auth(nullptr).error == AccountLoginError::invalid_input &&
                   account_module_login_pump(nullptr).error == AccountLoginError::invalid_input &&
                   account_module_login_copy_webauthn_challenge(nullptr, nullptr).error ==
                       SecureBufferError::invalid_input &&
                   account_module_login_notification_handle(nullptr) == nullptr &&
                   account_module_login_stop(&empty_instance, 1).error == AccountLoginError::invalid_input &&
                   empty_instance == nullptr,
               "account_module_empty_contract");
        void* login_instance = nullptr;
        const AccountLoginStatus login_started =
            account_module_login_start(&login_instance, L"Z:\\MaiCord-login-contract.profile");
        expect(login_started.ok() && login_started.phase == AccountLoginPhase::idle && login_instance &&
                   account_module_login_notification_handle(login_instance) == nullptr &&
                   account_module_login_stop(&login_instance, 5'000).phase == AccountLoginPhase::stopped &&
                   !login_instance,
               "account_module_login_lifecycle_contract");
        void* reset_instance = nullptr;
        const AccountLoginStatus reset_started =
            account_module_login_start_reset(&reset_instance, L"Z:\\MaiCord-reset-contract.profile");
        const bool reset_dormant = reset_started.ok() && reset_started.phase == AccountLoginPhase::idle &&
            reset_instance && !account_module_login_notification_handle(reset_instance);
        SecureBuffer empty_locked{};
        expect(reset_dormant &&
                   account_module_login_submit_password(reset_instance, &empty_locked, &empty_locked).error ==
                       AccountLoginError::invalid_input &&
                   account_module_login_submit_verification(reset_instance, &empty_locked).error ==
                       AccountLoginError::invalid_input &&
                   account_module_login_submit_recovery(reset_instance, &empty_locked).error ==
                       AccountLoginError::invalid_input &&
                   account_module_login_start_remote_auth(reset_instance).error == AccountLoginError::invalid_input &&
                   !account_module_login_notification_handle(reset_instance),
               "account_module_reset_owner_is_lazy_and_rejects_unrelated_operations_without_worker");
        expect(reset_instance && account_module_login_stop(&reset_instance, 5'000).phase == AccountLoginPhase::stopped &&
                   !reset_instance,
               "account_module_reset_dormant_owner_stops_without_credential_or_network");
    }
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_module_entry() noexcept {
    maicord::test_account_module_contract();
    maicord::test_account_module_dormant(maicord::expect);
    maicord::test_exit(maicord::failures, "Account module");
}
