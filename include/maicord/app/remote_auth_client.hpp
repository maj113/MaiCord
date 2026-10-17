#pragma once

#include "maicord/app/account_profile.hpp"
#include "maicord/app/captcha_challenge.hpp"
#include "maicord/app/remote_auth_handshake.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/user_auth.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

inline constexpr u32 kRemoteAuthQrUrlBytes = 66;
inline constexpr u32 kRemoteAuthClientUserBytes = kRemoteAuthMaximumUserPayloadBytes;

enum class RemoteAuthClientPhase : u8 {
    empty,
    starting,
    resolving,
    connecting,
    tls_handshake,
    websocket_opening,
    awaiting_qr,
    qr_ready,
    mobile_pending,
    ticket_exchange,
    captcha_required,
    authenticated,
    cancelling,
    cancelled,
    stopping,
    stopped,
    failed,
};

enum class RemoteAuthClientError : u8 {
    none,
    invalid_input,
    memory,
    event,
    thread,
    network,
    resolve,
    connect,
    tls,
    websocket,
    random,
    handshake,
    ticket_request,
    ticket_transport,
    ticket_response,
    captcha,
    credential,
    cancelled,
    stop,
};

enum class RemoteAuthClientEventKind : u8 {
    none,
    qr_ready,
    mobile_pending,
    captcha_required,
    authenticated,
    cancelled,
    failed,
    stopped,
};

struct RemoteAuthClientEvent {
    u64 next_wake_at_ms;
    u64 ticket_error_code;
    u64 retry_at_ms;
    u32 native_error;
    u32 ticket_response_bytes;
    u32 ticket_decode_error_offset;
    u16 http_status;
    RemoteAuthHandshakeError handshake_error;
    RemoteAuthHandshakeStage handshake_stage;
    RemoteAuthError protocol_error;
    RemoteAuthCryptoError crypto_error;
    Base64Error base64_error;
    UserAuthResponseKind ticket_response_kind;
    UserAuthDecodeError ticket_decode_error;
    JsonScanError ticket_scan_error;
    JsonReadError ticket_read_error;
    RestContentType ticket_content_type;
    RestContentEncoding ticket_content_encoding;
    RestBodyKind ticket_body_kind;
    CaptchaProvider captcha_provider;
    bool captcha_invisible;
    RemoteAuthClientEventKind kind;
    RemoteAuthClientError error;
};

struct RemoteAuthClientResult {
    u64 next_wake_at_ms;
    u64 ticket_error_code;
    u64 retry_at_ms;
    u32 native_error;
    u32 ticket_response_bytes;
    u32 ticket_decode_error_offset;
    u16 http_status;
    RemoteAuthHandshakeError handshake_error;
    RemoteAuthHandshakeStage handshake_stage;
    RemoteAuthError protocol_error;
    RemoteAuthCryptoError crypto_error;
    Base64Error base64_error;
    UserAuthResponseKind ticket_response_kind;
    UserAuthDecodeError ticket_decode_error;
    JsonScanError ticket_scan_error;
    JsonReadError ticket_read_error;
    RestContentType ticket_content_type;
    RestContentEncoding ticket_content_encoding;
    RestBodyKind ticket_body_kind;
    CaptchaProvider captcha_provider;
    bool captcha_invisible;
    RemoteAuthClientError error;
    RemoteAuthClientPhase phase;

    constexpr bool ok() const noexcept {
        return error == RemoteAuthClientError::none;
    }
};

struct RemoteAuthClient {
    void* runtime;
    HANDLE thread;
    HANDLE ready_event;
    HANDLE notification_event;
    HANDLE cancel_event;
    HANDLE captcha_event;
    SrwLock snapshot_lock;
    volatile LONG phase;
    volatile LONG error;
    volatile LONG native_error;
    volatile LONG ticket_response_bytes;
    volatile LONG ticket_decode_error_offset;
    volatile LONG64 ticket_error_code;
    volatile LONG64 retry_at_ms;
    volatile LONG http_status;
    volatile LONG handshake_error;
    volatile LONG handshake_stage;
    volatile LONG protocol_error;
    volatile LONG crypto_error;
    volatile LONG base64_error;
    volatile LONG ticket_response_kind;
    volatile LONG ticket_decode_error;
    volatile LONG ticket_scan_error;
    volatile LONG ticket_read_error;
    volatile LONG ticket_content_type;
    volatile LONG ticket_content_encoding;
    volatile LONG ticket_body_kind;
    volatile LONG captcha_provider;
    volatile LONG captcha_invisible;
    volatile LONG stop_requested;
    volatile LONG token_taken;
    volatile LONG64 next_wake_at_ms;
    char qr_url[kRemoteAuthQrUrlBytes];
    char user[kRemoteAuthClientUserBytes];
    u32 qr_url_bytes;
    u32 user_bytes;
};

// Starts a credential-free Remote Auth v2 owner. The worker owns its ephemeral
// key, Schannel/WebSocket session, one-time REST ticket exchange and locked
// final credential. No account token crosses this API except through the
// locked, one-shot take_credential operation.
RemoteAuthClientResult remote_auth_client_start(RemoteAuthClient* client) noexcept;
RemoteAuthClientResult remote_auth_client_cancel(RemoteAuthClient* client) noexcept;
// Stop joins both the QR thread and its nested ticket worker. On a stop error,
// retain the complete client and its address until a later stop retry succeeds.
RemoteAuthClientResult remote_auth_client_stop(RemoteAuthClient* client, u32 timeout_ms) noexcept;

bool remote_auth_client_pop_event(RemoteAuthClient* client, RemoteAuthClientEvent* event) noexcept;
bool remote_auth_client_copy_qr_url(const RemoteAuthClient& client, Slice<char> output,
                                    u32* bytes) noexcept;
bool remote_auth_client_copy_user(const RemoteAuthClient& client, Slice<char> output, u32* bytes) noexcept;
SecureBufferResult remote_auth_client_take_credential(RemoteAuthClient* client,
                                                      SecureBuffer* output) noexcept;
SecureBufferResult remote_auth_client_copy_captcha_secret(const RemoteAuthClient* client,
                                                          CaptchaChallengeSecretKind kind,
                                                          SecureBuffer* output) noexcept;
RemoteAuthClientResult remote_auth_client_submit_captcha(RemoteAuthClient* client,
                                                         const SecureBuffer* solution) noexcept;

// Borrowed readiness handle. An authenticated, unclaimed credential waits on
// the existing worker thread so publication cannot outrun final thread exit.
// Rebuild the wait set after pumping; stop releases all borrowed handles.
HANDLE remote_auth_client_notification_handle(const RemoteAuthClient* client) noexcept;
RemoteAuthClientResult remote_auth_client_status(const RemoteAuthClient* client) noexcept;
u32 remote_auth_client_runtime_bytes() noexcept;

} // namespace maicord
