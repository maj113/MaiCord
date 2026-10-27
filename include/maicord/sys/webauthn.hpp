#pragma once

// WebAuthn, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <webauthn.h>
#endif

// <webauthn.h>
#define WEBAUTHN_API_VERSION_1 1
#define WEBAUTHN_HASH_ALGORITHM_SHA_256 L"SHA-256"
#define WEBAUTHN_CLIENT_DATA_CURRENT_VERSION 1
#define WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY L"public-key"
#define WEBAUTHN_CTAP_TRANSPORT_INTERNAL 0x00000010
#define WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION 1
#define WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY 0
#define WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED 1
#define WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED 2
#define WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED 3
#define WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_4 4
#define WEBAUTHN_ASSERTION_VERSION_4 4

MAICORD_SYS_TYPES_BEGIN
// <webauthn.h>
struct _WEBAUTHN_CLIENT_DATA {
    DWORD dwVersion;
    DWORD cbClientDataJSON;
    PBYTE pbClientDataJSON;
    LPCWSTR pwszHashAlgId;
};
typedef _WEBAUTHN_CLIENT_DATA WEBAUTHN_CLIENT_DATA;
typedef const WEBAUTHN_CLIENT_DATA* PCWEBAUTHN_CLIENT_DATA;
struct _WEBAUTHN_CREDENTIAL {
    DWORD dwVersion;
    DWORD cbId;
    PBYTE pbId;
    PCWSTR pwszCredentialType;
};
typedef _WEBAUTHN_CREDENTIAL WEBAUTHN_CREDENTIAL;
typedef _WEBAUTHN_CREDENTIAL* PWEBAUTHN_CREDENTIAL;
struct _WEBAUTHN_CREDENTIALS {
    DWORD cCredentials;
    PWEBAUTHN_CREDENTIAL pCredentials;
};
typedef _WEBAUTHN_CREDENTIALS WEBAUTHN_CREDENTIALS;
struct _WEBAUTHN_CREDENTIAL_EX {
    DWORD dwVersion;
    DWORD cbId;
    PBYTE pbId;
    LPCWSTR pwszCredentialType;
    DWORD dwTransports;
};
typedef _WEBAUTHN_CREDENTIAL_EX WEBAUTHN_CREDENTIAL_EX;
typedef _WEBAUTHN_CREDENTIAL_EX* PWEBAUTHN_CREDENTIAL_EX;
struct _WEBAUTHN_CREDENTIAL_LIST {
    DWORD cCredentials;
    PWEBAUTHN_CREDENTIAL_EX* ppCredentials;
};
typedef _WEBAUTHN_CREDENTIAL_LIST WEBAUTHN_CREDENTIAL_LIST;
typedef _WEBAUTHN_CREDENTIAL_LIST* PWEBAUTHN_CREDENTIAL_LIST;
struct _CTAPCBOR_HYBRID_STORAGE_LINKED_DATA {
    DWORD dwVersion;
    DWORD cbContactId;
    PBYTE pbContactId;
    DWORD cbLinkId;
    PBYTE pbLinkId;
    DWORD cbLinkSecret;
    PBYTE pbLinkSecret;
    DWORD cbPublicKey;
    PBYTE pbPublicKey;
    PCWSTR pwszAuthenticatorName;
    WORD wEncodedTunnelServerDomain;
};
typedef _CTAPCBOR_HYBRID_STORAGE_LINKED_DATA* PCTAPCBOR_HYBRID_STORAGE_LINKED_DATA;
struct _WEBAUTHN_HMAC_SECRET_SALT {
    DWORD cbFirst;
    PBYTE pbFirst;
    DWORD cbSecond;
    PBYTE pbSecond;
};
typedef _WEBAUTHN_HMAC_SECRET_SALT* PWEBAUTHN_HMAC_SECRET_SALT;
struct _WEBAUTHN_CRED_WITH_HMAC_SECRET_SALT {
    DWORD cbCredID;
    PBYTE pbCredID;
    PWEBAUTHN_HMAC_SECRET_SALT pHmacSecretSalt;
};
typedef _WEBAUTHN_CRED_WITH_HMAC_SECRET_SALT* PWEBAUTHN_CRED_WITH_HMAC_SECRET_SALT;
struct _WEBAUTHN_HMAC_SECRET_SALT_VALUES {
    PWEBAUTHN_HMAC_SECRET_SALT pGlobalHmacSalt;
    DWORD cCredWithHmacSecretSaltList;
    PWEBAUTHN_CRED_WITH_HMAC_SECRET_SALT pCredWithHmacSecretSaltList;
};
typedef _WEBAUTHN_HMAC_SECRET_SALT_VALUES* PWEBAUTHN_HMAC_SECRET_SALT_VALUES;
struct _WEBAUTHN_EXTENSION {
    LPCWSTR pwszExtensionIdentifier;
    DWORD cbExtension;
    PVOID pvExtension;
};
typedef _WEBAUTHN_EXTENSION* PWEBAUTHN_EXTENSION;
struct _WEBAUTHN_EXTENSIONS {
    DWORD cExtensions;
    PWEBAUTHN_EXTENSION pExtensions;
};
typedef _WEBAUTHN_EXTENSIONS WEBAUTHN_EXTENSIONS;
struct _WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS {
    DWORD dwVersion;
    DWORD dwTimeoutMilliseconds;
    WEBAUTHN_CREDENTIALS CredentialList;
    WEBAUTHN_EXTENSIONS Extensions;
    DWORD dwAuthenticatorAttachment;
    DWORD dwUserVerificationRequirement;
    DWORD dwFlags;
    PCWSTR pwszU2fAppId;
    BOOL* pbU2fAppId;
    GUID* pCancellationId;
    PWEBAUTHN_CREDENTIAL_LIST pAllowCredentialList;
    DWORD dwCredLargeBlobOperation;
    DWORD cbCredLargeBlob;
    PBYTE pbCredLargeBlob;
    PWEBAUTHN_HMAC_SECRET_SALT_VALUES pHmacSecretSaltValues;
    BOOL bBrowserInPrivateMode;
    PCTAPCBOR_HYBRID_STORAGE_LINKED_DATA pLinkedDevice;
    BOOL bAutoFill;
    DWORD cbJsonExt;
    PBYTE pbJsonExt;
    DWORD cCredentialHints;
    LPCWSTR* ppwszCredentialHints;
    PCWSTR pwszRemoteWebOrigin;
    DWORD cbPublicKeyCredentialRequestOptionsJSON;
    PBYTE pbPublicKeyCredentialRequestOptionsJSON;
    DWORD cbAuthenticatorId;
    PBYTE pbAuthenticatorId;
};
typedef _WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS;
typedef const WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS* PCWEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS;
struct _WEBAUTHN_ASSERTION {
    DWORD dwVersion;
    DWORD cbAuthenticatorData;
    PBYTE pbAuthenticatorData;
    DWORD cbSignature;
    PBYTE pbSignature;
    WEBAUTHN_CREDENTIAL Credential;
    DWORD cbUserId;
    PBYTE pbUserId;
    WEBAUTHN_EXTENSIONS Extensions;
    DWORD cbCredLargeBlob;
    PBYTE pbCredLargeBlob;
    DWORD dwCredLargeBlobStatus;
    PWEBAUTHN_HMAC_SECRET_SALT pHmacSecret;
    DWORD dwUsedTransport;
    DWORD cbUnsignedExtensionOutputs;
    PBYTE pbUnsignedExtensionOutputs;
    DWORD cbClientDataJSON;
    PBYTE pbClientDataJSON;
    DWORD cbAuthenticationResponseJSON;
    PBYTE pbAuthenticationResponseJSON;
};
typedef _WEBAUTHN_ASSERTION* PWEBAUTHN_ASSERTION;
MAICORD_SYS_TYPES_END
