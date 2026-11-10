#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/platform/desktop/webauthn.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/win32.hpp"
#include "maicord/os/text.hpp"
#include "maicord/sys/webauthn.hpp"

namespace maicord {
namespace {

using GetApiVersionFunction = DWORD(WINAPI*)();
using GetAssertionFunction = HRESULT(WINAPI*)(
    HWND, LPCWSTR, PCWEBAUTHN_CLIENT_DATA,
    PCWEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS,
    PWEBAUTHN_ASSERTION*) noexcept;
using FreeAssertionFunction = void(WINAPI*)(PWEBAUTHN_ASSERTION) noexcept;
using GetCancellationIdFunction = HRESULT(WINAPI*)(GUID*) noexcept;
using MakeCredentialFunction = HRESULT(WINAPI*)(
    HWND, PCWEBAUTHN_RP_ENTITY_INFORMATION, PCWEBAUTHN_USER_ENTITY_INFORMATION,
    PCWEBAUTHN_COSE_CREDENTIAL_PARAMETERS, PCWEBAUTHN_CLIENT_DATA,
    PCWEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS,
    PWEBAUTHN_CREDENTIAL_ATTESTATION*) noexcept;
using FreeAttestationFunction = void(WINAPI*)(PWEBAUTHN_CREDENTIAL_ATTESTATION) noexcept;

WindowsWebAuthnResult failure(WindowsWebAuthnError error, HRESULT result = S_OK,
                              u32 api_version = 0) noexcept {
    return WindowsWebAuthnResult{static_cast<i32>(result), api_version, error};
}

DWORD verification_requirement(
    WebAuthnUserVerification verification) noexcept {
    switch (verification) {
    case WebAuthnUserVerification::required:
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
    case WebAuthnUserVerification::discouraged:
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
    default:
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
    }
}

bool valid_pointer(const void* pointer, u32 bytes) noexcept {
    return pointer || bytes == 0;
}

// What both ceremonies hold while the platform prompts: webauthn.dll, its two entry points of the ceremony, the client
// data and the allowed (or excluded) credentials as the platform's list. Filled as each step needs it, never cleared up
// front: close() wipes it.
struct Ceremony {
    HMODULE module;
    FARPROC call;
    FARPROC release;
    FARPROC cancellation_call;
    u32 api_version;
    bool has_cancellation;
    GUID cancellation_id;
    WEBAUTHN_CLIENT_DATA client;
    WEBAUTHN_CREDENTIAL_LIST list;
    u8 client_data[kWebAuthnMfaMaximumClientDataBytes];
    WEBAUTHN_CREDENTIAL_EX credentials[kWebAuthnMfaMaximumAllowedCredentials];
    PWEBAUTHN_CREDENTIAL_EX pointers[kWebAuthnMfaMaximumAllowedCredentials];
};

// The checks both ceremonies start with.
bool request_valid(HWND owner, const WebAuthnMfaRequestOptions& request, const SecureBuffer* credential) noexcept {
    return owner && IsWindow(owner) && credential && credential->data && credential->locked &&
           credential->capacity != 0 && request.challenge_bytes != 0 &&
           request.challenge_bytes <= kWebAuthnMfaMaximumChallengeBytes &&
           request.credential_count <= kWebAuthnMfaMaximumAllowedCredentials &&
           request.credential_id_bytes <= kWebAuthnMfaCredentialIdStorageBytes;
}

// Loads webauthn.dll and the entry points `call` and `release` (the library stays loaded on success). The result's error
// is none when the ceremony can run, else why it cannot.
WindowsWebAuthnResult open(Ceremony& c, const char* call, const char* release) noexcept {
    c.module = LoadLibraryExW(L"webauthn.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!c.module) {
        return failure(WindowsWebAuthnError::unavailable, HRESULT_FROM_WIN32(thread_last_error()));
    }
    const auto get_api_version =
        reinterpret_cast<GetApiVersionFunction>(GetProcAddress(c.module, "WebAuthNGetApiVersionNumber"));
    c.call = GetProcAddress(c.module, call);
    c.release = GetProcAddress(c.module, release);
    c.cancellation_call = GetProcAddress(c.module, "WebAuthNGetCancellationId");
    if (!get_api_version || !c.call || !c.release) {
        FreeLibrary(c.module);
        return failure(WindowsWebAuthnError::unavailable, HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    }
    c.api_version = get_api_version();
    if (c.api_version < WEBAUTHN_API_VERSION_1) {
        FreeLibrary(c.module);
        return failure(WindowsWebAuthnError::api_version, E_NOTIMPL, c.api_version);
    }
    return failure(WindowsWebAuthnError::none);
}

// Frees the library and wipes the client data, credentials and cancellation id.
void close(Ceremony& c) noexcept {
    FreeLibrary(c.module);
    SecureZeroMemory(&c, sizeof(c));
}

// The client data (`written`) and the request's credentials as the platform's list, the cancellation id and the client
// data structure both calls take. The result's error is none when the call can be made.
WindowsWebAuthnResult prepare(Ceremony& c, const WebAuthnMfaRequestOptions& request,
                              const WebAuthnMfaResult& written) noexcept {
    if (!written.ok()) {
        return failure(WindowsWebAuthnError::client_data, E_INVALIDARG, c.api_version);
    }
    for (u32 index = 0; index < request.credential_count; ++index) {
        const WebAuthnAllowedCredential& source = request.credentials[index];
        if (source.id_bytes == 0 || source.id_bytes > kWebAuthnMfaMaximumCredentialIdBytes ||
            source.id_offset > request.credential_id_bytes ||
            source.id_bytes > request.credential_id_bytes - source.id_offset) {
            return failure(WindowsWebAuthnError::invalid_input, E_INVALIDARG, c.api_version);
        }
        c.credentials[index] = {WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION, source.id_bytes,
                                const_cast<PBYTE>(request.credential_ids + source.id_offset),
                                WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, source.transports};
        c.pointers[index] = &c.credentials[index];
    }
    c.list = {request.credential_count, c.pointers};
    const auto get_cancellation_id = reinterpret_cast<GetCancellationIdFunction>(c.cancellation_call);
    c.has_cancellation = get_cancellation_id && SUCCEEDED(get_cancellation_id(&c.cancellation_id));
    c.client = {WEBAUTHN_CLIENT_DATA_CURRENT_VERSION, written.bytes, c.client_data, WEBAUTHN_HASH_ALGORITHM_SHA_256};
    return failure(WindowsWebAuthnError::none);
}

// A failed platform call by cause. Only a registration can meet an excluded credential (NTE_EXISTS: the key holds one) or
// a key without the algorithms (NTE_NOT_SUPPORTED).
WindowsWebAuthnError platform_error(HRESULT failed, bool registering) noexcept {
    if (failed == HRESULT_FROM_WIN32(ERROR_CANCELLED) || failed == NTE_USER_CANCELLED)
        return WindowsWebAuthnError::cancelled;
    if (failed == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        return WindowsWebAuthnError::timed_out;
    if (registering && failed == static_cast<HRESULT>(0x8009000Fu))
        return WindowsWebAuthnError::excluded;
    if (registering && failed == static_cast<HRESULT>(0x80090029u))
        return WindowsWebAuthnError::unsupported;
    return WindowsWebAuthnError::platform;
}

// The credential JSON `serialized` described, now in the caller's buffer, or why it could not be written.
WindowsWebAuthnResult store(SecureBuffer* credential, const WebAuthnMfaResult& serialized, u32 api_version) noexcept {
    if (!serialized.ok()) {
        return failure(serialized.error == WebAuthnMfaError::output_too_small ? WindowsWebAuthnError::output_too_small
                                                                              : WindowsWebAuthnError::assertion,
                       E_FAIL, api_version);
    }
    credential->bytes = serialized.bytes;
    return failure(WindowsWebAuthnError::none, S_OK, api_version);
}

} // namespace

WindowsWebAuthnResult windows_webauthn_authenticate(
    Window window, const WebAuthnMfaRequestOptions& request,
    SecureBuffer* serialized_credential) noexcept {
    const HWND owner = win32_hwnd(window);
    if (!request_valid(owner, request, serialized_credential)) {
        return failure(WindowsWebAuthnError::invalid_input, E_INVALIDARG);
    }
    secure_buffer_clear(serialized_credential);
    Ceremony c;
    WindowsWebAuthnResult result = open(c, "WebAuthNAuthenticatorGetAssertion", "WebAuthNFreeAssertion");
    if (!result.ok()) {
        return result;
    }
    const WebAuthnMfaResult client_data = webauthn_mfa_write_client_data(
        request, Slice<u8>{c.client_data, static_cast<u32>(sizeof(c.client_data))});
    result = prepare(c, request, client_data);
    if (result.ok()) {
        WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS options{};
        // API version 1 already supports options version 4. Version 4 is the
        // oldest structure that retains per-credential transport hints.
        options.dwVersion = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_4;
        options.dwTimeoutMilliseconds = request.timeout_ms;
        options.dwAuthenticatorAttachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
        options.dwUserVerificationRequirement = verification_requirement(request.user_verification);
        options.pCancellationId = c.has_cancellation ? &c.cancellation_id : nullptr;
        options.pAllowCredentialList = request.credential_count == 0 ? nullptr : &c.list;
        PWEBAUTHN_ASSERTION assertion = nullptr;
        const HRESULT asserted = reinterpret_cast<GetAssertionFunction>(c.call)(
            owner, L"discord.com", &c.client, &options, &assertion);
        if (FAILED(asserted)) {
            result = failure(platform_error(asserted, false), asserted, c.api_version);
        } else if (!assertion || assertion->Credential.cbId == 0 ||
                   !valid_pointer(assertion->Credential.pbId, assertion->Credential.cbId) ||
                   assertion->cbAuthenticatorData == 0 ||
                   !valid_pointer(assertion->pbAuthenticatorData, assertion->cbAuthenticatorData) ||
                   assertion->cbSignature == 0 ||
                   !valid_pointer(assertion->pbSignature, assertion->cbSignature) ||
                   !valid_pointer(assertion->pbUserId, assertion->cbUserId)) {
            result = failure(WindowsWebAuthnError::assertion, E_FAIL, c.api_version);
        } else {
            WebAuthnAuthenticatorAttachment attachment = WebAuthnAuthenticatorAttachment::unknown;
            if (assertion->dwVersion >= WEBAUTHN_ASSERTION_VERSION_4) {
                attachment = (assertion->dwUsedTransport & WEBAUTHN_CTAP_TRANSPORT_INTERNAL) != 0
                                 ? WebAuthnAuthenticatorAttachment::platform
                                 : WebAuthnAuthenticatorAttachment::cross_platform;
            }
            const WebAuthnMfaAssertionView view{
                ConstSlice<u8>{assertion->Credential.pbId, assertion->Credential.cbId},
                ConstSlice<u8>{c.client_data, client_data.bytes},
                ConstSlice<u8>{assertion->pbAuthenticatorData, assertion->cbAuthenticatorData},
                ConstSlice<u8>{assertion->pbSignature, assertion->cbSignature},
                ConstSlice<u8>{assertion->pbUserId, assertion->cbUserId},
                attachment};
            result = store(serialized_credential,
                           webauthn_mfa_write_assertion(
                               view, Slice<u8>{serialized_credential->data, serialized_credential->capacity}),
                           c.api_version);
        }
        if (assertion) {
            reinterpret_cast<FreeAssertionFunction>(c.release)(assertion);
        }
    }
    close(c);
    if (!result.ok()) {
        secure_buffer_clear(serialized_credential);
    }
    return result;
}

WindowsWebAuthnResult windows_webauthn_register(
    Window window, const WebAuthnMfaCreationOptions& creation,
    SecureBuffer* serialized_credential) noexcept {
    const WebAuthnMfaRequestOptions& request = creation.request;
    const HWND owner = win32_hwnd(window);
    if (!request_valid(owner, request, serialized_credential) ||
        creation.user_id_bytes == 0 || creation.user_id_bytes > kWebAuthnMfaMaximumUserIdBytes ||
        creation.algorithm_count == 0 ||
        creation.algorithm_count > kWebAuthnMfaMaximumAlgorithms) {
        return failure(WindowsWebAuthnError::invalid_input, E_INVALIDARG);
    }
    // The algorithms this ceremony asks Windows for: the server's ES256 and RS256, in its order.
    WEBAUTHN_COSE_CREDENTIAL_PARAMETER parameters[kWebAuthnMfaMaximumAlgorithms];
    u32 parameter_count = 0;
    for (u32 index = 0; index < creation.algorithm_count; ++index) {
        const i32 algorithm = creation.algorithms[index];
        if (algorithm == WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256 ||
            algorithm == WEBAUTHN_COSE_ALGORITHM_RSASSA_PKCS1_V1_5_WITH_SHA256) {
            parameters[parameter_count++] = {WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION,
                                             WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, algorithm};
        }
    }
    if (parameter_count == 0) {
        return failure(WindowsWebAuthnError::unsupported, E_NOTIMPL);
    }
    secure_buffer_clear(serialized_credential);
    Ceremony c;
    WindowsWebAuthnResult result =
        open(c, "WebAuthNAuthenticatorMakeCredential", "WebAuthNFreeCredentialAttestation");
    if (!result.ok()) {
        return result;
    }
    const WebAuthnMfaResult client_data = webauthn_mfa_write_creation_client_data(
        creation, Slice<u8>{c.client_data, static_cast<u32>(sizeof(c.client_data))});
    result = prepare(c, request, client_data);
    u8 user_id[kWebAuthnMfaMaximumUserIdBytes];
    wchar_t rp_name[kWebAuthnMfaMaximumEntityNameBytes + 1];
    wchar_t user_name[kWebAuthnMfaMaximumEntityNameBytes + 1];
    wchar_t display_name[kWebAuthnMfaMaximumEntityNameBytes + 1];
    if (result.ok()) {
        // Each name as UTF-16 (NUL terminated), or `fallback` when it is empty.
        const auto widen = [](const char* text, wchar_t* output, const wchar_t* fallback) noexcept {
            constexpr u32 capacity = kWebAuthnMfaMaximumEntityNameBytes + 1;
            const u32 bytes = static_cast<u32>(c_string_length(text));
            const u32 units = bytes ? utf8_to_utf16(text, bytes, output, capacity - 1, false) : 0;
            if (units != 0) {
                output[units] = 0;
                return;
            }
            u32 at = 0;
            for (; fallback[at] && at + 1 < capacity; ++at) {
                output[at] = fallback[at];
            }
            output[at] = 0;
        };
        widen(creation.rp_name, rp_name, L"Discord");
        widen(creation.user_name, user_name, L"Discord");
        widen(creation.user_display_name, display_name, user_name);
        copy_bytes(user_id, creation.user_id, creation.user_id_bytes);

        WEBAUTHN_RP_ENTITY_INFORMATION rp{};
        rp.dwVersion = WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION;
        rp.pwszId = L"discord.com";
        rp.pwszName = rp_name;
        WEBAUTHN_USER_ENTITY_INFORMATION user{};
        user.dwVersion = WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION;
        user.cbId = creation.user_id_bytes;
        user.pbId = user_id;
        user.pwszName = user_name;
        user.pwszDisplayName = display_name;
        WEBAUTHN_COSE_CREDENTIAL_PARAMETERS algorithms{parameter_count, parameters};

        WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS options{};
        // Options version 4 carries the exclude list (3) and the resident key preference (4).
        options.dwVersion = WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_4;
        options.dwTimeoutMilliseconds = request.timeout_ms;
        options.dwAuthenticatorAttachment =
            creation.attachment == WebAuthnAuthenticatorAttachment::platform
                ? WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM
            : creation.attachment == WebAuthnAuthenticatorAttachment::cross_platform
                ? WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM
                : WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
        options.bRequireResidentKey = creation.resident_key == WebAuthnResidentKey::required;
        options.bPreferResidentKey = creation.resident_key == WebAuthnResidentKey::preferred;
        options.dwUserVerificationRequirement = verification_requirement(request.user_verification);
        options.dwAttestationConveyancePreference = creation.attestation_none
                                                        ? WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE
                                                        : WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT;
        options.pCancellationId = c.has_cancellation ? &c.cancellation_id : nullptr;
        options.pExcludeCredentialList = request.credential_count == 0 ? nullptr : &c.list;
        PWEBAUTHN_CREDENTIAL_ATTESTATION attestation = nullptr;
        const HRESULT created = reinterpret_cast<MakeCredentialFunction>(c.call)(
            owner, &rp, &user, &algorithms, &c.client, &options, &attestation);
        if (FAILED(created)) {
            result = failure(platform_error(created, true), created, c.api_version);
        } else if (!attestation || attestation->cbCredentialId == 0 ||
                   !valid_pointer(attestation->pbCredentialId, attestation->cbCredentialId) ||
                   attestation->cbAttestationObject == 0 ||
                   !valid_pointer(attestation->pbAttestationObject, attestation->cbAttestationObject) ||
                   attestation->cbAuthenticatorData == 0 ||
                   !valid_pointer(attestation->pbAuthenticatorData, attestation->cbAuthenticatorData)) {
            result = failure(WindowsWebAuthnError::assertion, E_FAIL, c.api_version);
        } else {
            const ConstSlice<u8> authenticator_data{attestation->pbAuthenticatorData,
                                                    attestation->cbAuthenticatorData};
            i32 algorithm = webauthn_mfa_credential_algorithm(authenticator_data);
            if (algorithm == 0) {
                algorithm = creation.algorithms[0];
            }
            // Windows reports every transport from version 8, else the one it used (version 3).
            constexpr u32 kKnownTransports = WEBAUTHN_CTAP_TRANSPORT_USB | WEBAUTHN_CTAP_TRANSPORT_NFC |
                                             WEBAUTHN_CTAP_TRANSPORT_BLE | WEBAUTHN_CTAP_TRANSPORT_INTERNAL |
                                             WEBAUTHN_CTAP_TRANSPORT_HYBRID;
            const u32 transports = (attestation->dwVersion >= 8   ? attestation->dwTransports
                                    : attestation->dwVersion >= 3 ? attestation->dwUsedTransport
                                                                  : 0u) &
                                   kKnownTransports;
            const WebAuthnAuthenticatorAttachment attachment =
                attestation->dwVersion < 3 ? WebAuthnAuthenticatorAttachment::unknown
                : (attestation->dwUsedTransport & WEBAUTHN_CTAP_TRANSPORT_INTERNAL) != 0
                    ? WebAuthnAuthenticatorAttachment::platform
                    : WebAuthnAuthenticatorAttachment::cross_platform;
            const WebAuthnMfaAttestationView view{
                ConstSlice<u8>{attestation->pbCredentialId, attestation->cbCredentialId},
                ConstSlice<u8>{c.client_data, client_data.bytes},
                ConstSlice<u8>{attestation->pbAttestationObject, attestation->cbAttestationObject},
                authenticator_data,
                algorithm,
                transports,
                attachment};
            result = store(serialized_credential,
                           webauthn_mfa_write_attestation(
                               view, Slice<u8>{serialized_credential->data, serialized_credential->capacity}),
                           c.api_version);
        }
        if (attestation) {
            reinterpret_cast<FreeAttestationFunction>(c.release)(attestation);
        }
        SecureZeroMemory(user_id, sizeof(user_id));
    }
    close(c);
    if (!result.ok()) {
        secure_buffer_clear(serialized_credential);
    }
    return result;
}

} // namespace maicord
