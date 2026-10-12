#pragma once

#include "maicord/app/account_profile.hpp"
#include "maicord/app/captcha_challenge.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/remote_auth_handshake.hpp"

namespace maicord {

inline constexpr u32 kRemoteAuthTicketExchangeRequestBytes = 1024;
inline constexpr u32 kRemoteAuthTicketExchangeResponseBytes = kUserAuthMaximumResponseBytes;
inline constexpr u32 kRemoteAuthTicketExchangeCredentialBytes = kAccountProfileMaximumCredentialBytes;
inline constexpr u32 kRemoteAuthTicketExchangeSolutionBytes = kUserAuthMaximumCaptchaKeyBytes;
inline constexpr u32 kRemoteAuthTicketExchangeStructuralCount =
    kRemoteAuthTicketExchangeSolutionBytes / sizeof(u32);
static_assert(kRemoteAuthTicketExchangeStructuralCount == 1'024);

enum class RemoteAuthTicketExchangeError : u8 {
    none,
    invalid_input,
    memory,
    request,
    transport,
    response,
    captcha,
    credential,
    cancelled,
    stop,
};

struct RemoteAuthTicketExchangeResult {
    u64 error_code;
    u32 native_error;
    u32 response_bytes;
    u32 decode_error_offset;
    u16 http_status;
    RemoteAuthHandshakeResult handshake;
    UserAuthResponseKind response_kind;
    UserAuthDecodeError decode_error;
    JsonScanError scan_error;
    JsonReadError read_error;
    RestContentType content_type;
    RestContentEncoding content_encoding;
    RestBodyKind body_kind;
    CaptchaProvider captcha_provider;
    bool captcha_invisible;
    RemoteAuthTicketExchangeError error;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthTicketExchangeError::none;
    }
};

enum class RemoteAuthTicketExchangeEventKind : u8 {
    captcha_required,
    retrying,
};

struct RemoteAuthTicketExchangeEvent {
    RemoteAuthTicketExchangeResult status;
    RemoteAuthTicketExchangeEventKind kind;
};

using RemoteAuthTicketExchangeEventFunction = void (*)(void* context,
                                                       const RemoteAuthTicketExchangeEvent& event) noexcept;

struct RemoteAuthTicketExchange {
    RestClient transport;
    SecureBuffer request;
    SecureBuffer credential;
    SecureBuffer captcha_solution;
    CaptchaChallenge captcha;
    volatile LONG captcha_submitted;
};

RemoteAuthTicketExchangeResult remote_auth_ticket_exchange_initialize(
    RemoteAuthTicketExchange* exchange) noexcept;
RemoteAuthTicketExchangeResult remote_auth_ticket_exchange_run(
    RemoteAuthTicketExchange* exchange, RemoteAuthHandshake* handshake, Utf8View ticket,
    HANDLE captcha_event, HANDLE cancel_event, RemoteAuthTicketExchangeEventFunction event_function,
    void* event_context) noexcept;
RemoteAuthTicketExchangeResult remote_auth_ticket_exchange_stop(RemoteAuthTicketExchange* exchange,
                                                                u32 timeout_ms) noexcept;
// A failed transport join erases caller secrets but retains the complete exchange.
// Preserve its address while transport.runtime/thread remain; retry stop or
// release before discarding the enclosing owner.
void remote_auth_ticket_exchange_release(RemoteAuthTicketExchange* exchange) noexcept;

SecureBufferResult remote_auth_ticket_exchange_copy_captcha_secret(const RemoteAuthTicketExchange* exchange,
                                                                   CaptchaChallengeSecretKind kind,
                                                                   SecureBuffer* output) noexcept;
SecureBufferResult remote_auth_ticket_exchange_submit_captcha(RemoteAuthTicketExchange* exchange,
                                                              const SecureBuffer* solution,
                                                              HANDLE captcha_event) noexcept;
SecureBufferResult remote_auth_ticket_exchange_take_credential(RemoteAuthTicketExchange* exchange,
                                                               SecureBuffer* output) noexcept;

u32 remote_auth_ticket_exchange_runtime_bytes() noexcept;

} // namespace maicord
