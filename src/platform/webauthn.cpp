#include "maicord/core/win32_process.hpp"
#include "maicord/platform/webauthn.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
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
    case WebAuthnUserVerification::preferred:
    default:
        return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
    }
}

bool valid_pointer(const void* pointer, u32 bytes) noexcept {
    return pointer || bytes == 0;
}

} // namespace

WindowsWebAuthnResult windows_webauthn_authenticate(
    HWND owner, const WebAuthnMfaRequestOptions& request,
    SecureBuffer* serialized_credential) noexcept {
    if (!owner || !IsWindow(owner) || !serialized_credential ||
        !serialized_credential->data || !serialized_credential->locked ||
        serialized_credential->capacity == 0 || request.challenge_bytes == 0 ||
        request.challenge_bytes > kWebAuthnMfaMaximumChallengeBytes ||
        request.credential_count > kWebAuthnMfaMaximumAllowedCredentials ||
        request.credential_id_bytes > kWebAuthnMfaCredentialIdStorageBytes) {
        return failure(WindowsWebAuthnError::invalid_input,
                       E_INVALIDARG);
    }
    secure_buffer_clear(serialized_credential);
    HMODULE module = LoadLibraryExW(L"webauthn.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        return failure(WindowsWebAuthnError::unavailable,
                       HRESULT_FROM_WIN32(thread_last_error()));
    }
    const auto get_api_version = reinterpret_cast<GetApiVersionFunction>(
        GetProcAddress(module, "WebAuthNGetApiVersionNumber"));
    const auto get_assertion = reinterpret_cast<GetAssertionFunction>(
        GetProcAddress(module, "WebAuthNAuthenticatorGetAssertion"));
    const auto free_assertion = reinterpret_cast<FreeAssertionFunction>(
        GetProcAddress(module, "WebAuthNFreeAssertion"));
    const auto get_cancellation_id =
        reinterpret_cast<GetCancellationIdFunction>(
            GetProcAddress(module, "WebAuthNGetCancellationId"));
    if (!get_api_version || !get_assertion || !free_assertion) {
        FreeLibrary(module);
        return failure(WindowsWebAuthnError::unavailable,
                       HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    }
    const u32 api_version = get_api_version();
    if (api_version < WEBAUTHN_API_VERSION_1) {
        FreeLibrary(module);
        return failure(WindowsWebAuthnError::api_version, E_NOTIMPL,
                       api_version);
    }

    u8 client_data_bytes[kWebAuthnMfaMaximumClientDataBytes]{};
    const WebAuthnMfaResult client_data = webauthn_mfa_write_client_data(
        request,
        Slice<u8>{client_data_bytes,
                  static_cast<u32>(sizeof(client_data_bytes))});
    if (!client_data.ok()) {
        SecureZeroMemory(client_data_bytes, sizeof(client_data_bytes));
        FreeLibrary(module);
        return failure(WindowsWebAuthnError::client_data, E_INVALIDARG,
                       api_version);
    }

    WEBAUTHN_CREDENTIAL_EX
        credentials[kWebAuthnMfaMaximumAllowedCredentials]{};
    PWEBAUTHN_CREDENTIAL_EX
        credential_pointers[kWebAuthnMfaMaximumAllowedCredentials]{};
    for (u32 index = 0; index < request.credential_count; ++index) {
        const WebAuthnAllowedCredential& source =
            request.credentials[index];
        if (source.id_bytes == 0 ||
            source.id_bytes > kWebAuthnMfaMaximumCredentialIdBytes ||
            source.id_offset > request.credential_id_bytes ||
            source.id_bytes >
                request.credential_id_bytes - source.id_offset) {
            SecureZeroMemory(client_data_bytes, sizeof(client_data_bytes));
            FreeLibrary(module);
            return failure(WindowsWebAuthnError::invalid_input,
                           E_INVALIDARG, api_version);
        }
        credentials[index].dwVersion = WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION;
        credentials[index].cbId = source.id_bytes;
        credentials[index].pbId = const_cast<PBYTE>(
            request.credential_ids + source.id_offset);
        credentials[index].pwszCredentialType =
            WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY;
        credentials[index].dwTransports = source.transports;
        credential_pointers[index] = &credentials[index];
    }
    WEBAUTHN_CREDENTIAL_LIST allow_list{
        request.credential_count, credential_pointers};
    GUID cancellation_id{};
    const bool has_cancellation =
        get_cancellation_id && SUCCEEDED(get_cancellation_id(&cancellation_id));
    WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS options{};
    // API version 1 already supports options version 4. Version 4 is the
    // oldest structure that retains per-credential transport hints.
    options.dwVersion =
        WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_4;
    options.dwTimeoutMilliseconds = request.timeout_ms;
    options.dwAuthenticatorAttachment =
        WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    options.dwUserVerificationRequirement =
        verification_requirement(request.user_verification);
    options.pCancellationId =
        has_cancellation ? &cancellation_id : nullptr;
    options.pAllowCredentialList =
        request.credential_count == 0 ? nullptr : &allow_list;
    WEBAUTHN_CLIENT_DATA webauthn_client_data{};
    webauthn_client_data.dwVersion = WEBAUTHN_CLIENT_DATA_CURRENT_VERSION;
    webauthn_client_data.cbClientDataJSON = client_data.bytes;
    webauthn_client_data.pbClientDataJSON = client_data_bytes;
    webauthn_client_data.pwszHashAlgId = WEBAUTHN_HASH_ALGORITHM_SHA_256;

    PWEBAUTHN_ASSERTION assertion = nullptr;
    const HRESULT asserted = get_assertion(
        owner, L"discord.com", &webauthn_client_data, &options,
        &assertion);
    WindowsWebAuthnResult result{};
    if (FAILED(asserted)) {
        const WindowsWebAuthnError error =
            asserted == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
                    asserted == NTE_USER_CANCELLED
                ? WindowsWebAuthnError::cancelled
            : asserted == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                ? WindowsWebAuthnError::timed_out
                : WindowsWebAuthnError::platform;
        result = failure(error, asserted, api_version);
    } else if (!assertion || assertion->Credential.cbId == 0 ||
               !valid_pointer(assertion->Credential.pbId,
                              assertion->Credential.cbId) ||
               assertion->cbAuthenticatorData == 0 ||
               !valid_pointer(assertion->pbAuthenticatorData,
                              assertion->cbAuthenticatorData) ||
               assertion->cbSignature == 0 ||
               !valid_pointer(assertion->pbSignature,
                              assertion->cbSignature) ||
               !valid_pointer(assertion->pbUserId,
                              assertion->cbUserId)) {
        result = failure(WindowsWebAuthnError::assertion, E_FAIL,
                         api_version);
    } else {
        WebAuthnAuthenticatorAttachment attachment =
            WebAuthnAuthenticatorAttachment::unknown;
        if (assertion->dwVersion >= WEBAUTHN_ASSERTION_VERSION_4) {
            attachment =
                (assertion->dwUsedTransport &
                 WEBAUTHN_CTAP_TRANSPORT_INTERNAL) != 0
                    ? WebAuthnAuthenticatorAttachment::platform
                    : WebAuthnAuthenticatorAttachment::cross_platform;
        }
        const WebAuthnMfaAssertionView view{
            ConstSlice<u8>{assertion->Credential.pbId,
                           assertion->Credential.cbId},
            ConstSlice<u8>{client_data_bytes, client_data.bytes},
            ConstSlice<u8>{assertion->pbAuthenticatorData,
                           assertion->cbAuthenticatorData},
            ConstSlice<u8>{assertion->pbSignature,
                           assertion->cbSignature},
            ConstSlice<u8>{assertion->pbUserId, assertion->cbUserId},
            attachment};
        const WebAuthnMfaResult serialized = webauthn_mfa_write_assertion(
            view,
            Slice<u8>{serialized_credential->data,
                      serialized_credential->capacity});
        if (!serialized.ok()) {
            result = failure(
                serialized.error == WebAuthnMfaError::output_too_small
                    ? WindowsWebAuthnError::output_too_small
                    : WindowsWebAuthnError::assertion,
                E_FAIL, api_version);
        } else {
            serialized_credential->bytes = serialized.bytes;
            result = failure(WindowsWebAuthnError::none, S_OK,
                             api_version);
        }
    }
    if (assertion) {
        free_assertion(assertion);
    }
    SecureZeroMemory(client_data_bytes, sizeof(client_data_bytes));
    SecureZeroMemory(credentials, sizeof(credentials));
    SecureZeroMemory(credential_pointers, sizeof(credential_pointers));
    SecureZeroMemory(&cancellation_id, sizeof(cancellation_id));
    FreeLibrary(module);
    if (!result.ok()) {
        secure_buffer_clear(serialized_credential);
    }
    return result;
}

} // namespace maicord
