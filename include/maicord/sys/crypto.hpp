#pragma once

// CNG (bcrypt, ncrypt) and DPAPI, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <dpapi.h>
#endif

// <bcrypt.h>
#define BCRYPT_KDF_RAW_SECRET L"TRUNCATE"
#define BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION 1
#define BCRYPT_INIT_AUTH_MODE_INFO(_AUTH_INFO_STRUCT_) RtlZeroMemory((&_AUTH_INFO_STRUCT_), sizeof(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO)); (_AUTH_INFO_STRUCT_).cbSize = sizeof(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO); (_AUTH_INFO_STRUCT_).dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
#define BCRYPT_OBJECT_LENGTH L"ObjectLength"
#define BCRYPT_PAD_OAEP 0x00000004
#define BCRYPT_RSAPUBLIC_BLOB L"RSAPUBLICBLOB"
#define BCRYPT_RSAPUBLIC_MAGIC 0x31415352
#define BCRYPT_ECCPUBLIC_BLOB L"ECCPUBLICBLOB"
#define BCRYPT_ECCPRIVATE_BLOB L"ECCPRIVATEBLOB"
#define BCRYPT_ECDH_PUBLIC_P256_MAGIC 0x314B4345
#define BCRYPT_ECDH_PRIVATE_P256_MAGIC 0x324B4345
#define BCRYPT_ECDSA_PUBLIC_P256_MAGIC 0x31534345
#define BCRYPT_ECDSA_PRIVATE_P256_MAGIC 0x32534345
#define BCRYPT_RSA_ALGORITHM L"RSA"
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#define BCRYPT_MD5_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x00000021)
#define BCRYPT_SHA256_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x00000041)
#define BCRYPT_HMAC_SHA256_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x000000b1)
#define BCRYPT_AES_ECB_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x000001b1)
#define BCRYPT_AES_GCM_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x000001e1)
#define BCRYPT_ECDH_P256_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x000002a1)
#define BCRYPT_ECDSA_P256_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x000002e1)
#define BCRYPT_CHACHA20_POLY1305_ALG_HANDLE ((BCRYPT_ALG_HANDLE) 0x000003A1)
#define BCRYPT_HASH_REUSABLE_FLAG 0x00000020
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002

// <dpapi.h>
#define CRYPTPROTECT_UI_FORBIDDEN 0x1

typedef LONG NTSTATUS;
typedef PVOID BCRYPT_HANDLE;
typedef PVOID BCRYPT_ALG_HANDLE;
typedef PVOID BCRYPT_KEY_HANDLE;
typedef PVOID BCRYPT_HASH_HANDLE;
typedef PVOID BCRYPT_SECRET_HANDLE;
typedef unsigned int ALG_ID;
typedef void* HCERTSTORE;

MAICORD_SYS_TYPES_BEGIN
// <bcrypt.h>
struct _BCRYPT_OAEP_PADDING_INFO {
    LPCWSTR pszAlgId;
    PUCHAR pbLabel;
    ULONG cbLabel;
};
typedef _BCRYPT_OAEP_PADDING_INFO BCRYPT_OAEP_PADDING_INFO;
struct _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO {
    ULONG cbSize;
    ULONG dwInfoVersion;
    PUCHAR pbNonce;
    ULONG cbNonce;
    PUCHAR pbAuthData;
    ULONG cbAuthData;
    PUCHAR pbTag;
    ULONG cbTag;
    PUCHAR pbMacContext;
    ULONG cbMacContext;
    ULONG cbAAD;
    ULONGLONG cbData;
    ULONG dwFlags;
};
typedef _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO;
struct _BCryptBuffer {
    ULONG cbBuffer;
    ULONG BufferType;
    PVOID pvBuffer;
};
typedef _BCryptBuffer* PBCryptBuffer;
struct _BCryptBufferDesc {
    ULONG ulVersion;
    ULONG cBuffers;
    PBCryptBuffer pBuffers;
};
typedef _BCryptBufferDesc BCryptBufferDesc;
struct _BCRYPT_RSAKEY_BLOB {
    ULONG Magic;
    ULONG BitLength;
    ULONG cbPublicExp;
    ULONG cbModulus;
    ULONG cbPrime1;
    ULONG cbPrime2;
};
typedef _BCRYPT_RSAKEY_BLOB BCRYPT_RSAKEY_BLOB;
struct _BCRYPT_ECCKEY_BLOB {
    ULONG dwMagic;
    ULONG cbKey;
};
typedef _BCRYPT_ECCKEY_BLOB BCRYPT_ECCKEY_BLOB;
// <dpapi.h>
struct _CRYPTOAPI_BLOB {
    DWORD cbData;
    BYTE* pbData;
};
typedef _CRYPTOAPI_BLOB CRYPT_INTEGER_BLOB;
typedef _CRYPTOAPI_BLOB CRYPT_OBJID_BLOB;
typedef _CRYPTOAPI_BLOB CERT_NAME_BLOB;
typedef _CRYPTOAPI_BLOB DATA_BLOB;
struct _CRYPTPROTECT_PROMPTSTRUCT {
    DWORD cbSize;
    DWORD dwPromptFlags;
    HWND hwndApp;
    LPCWSTR szPrompt;
};
typedef _CRYPTPROTECT_PROMPTSTRUCT CRYPTPROTECT_PROMPTSTRUCT;
// <wincrypt.h>
struct _CRYPT_BIT_BLOB {
    DWORD cbData;
    BYTE* pbData;
    DWORD cUnusedBits;
};
typedef _CRYPT_BIT_BLOB CRYPT_BIT_BLOB;
struct _CRYPT_ALGORITHM_IDENTIFIER {
    LPSTR pszObjId;
    CRYPT_OBJID_BLOB Parameters;
};
typedef _CRYPT_ALGORITHM_IDENTIFIER CRYPT_ALGORITHM_IDENTIFIER;
struct _CERT_EXTENSION {
    LPSTR pszObjId;
    BOOL fCritical;
    CRYPT_OBJID_BLOB Value;
};
typedef _CERT_EXTENSION* PCERT_EXTENSION;
struct _CERT_PUBLIC_KEY_INFO {
    CRYPT_ALGORITHM_IDENTIFIER Algorithm;
    CRYPT_BIT_BLOB PublicKey;
};
typedef _CERT_PUBLIC_KEY_INFO CERT_PUBLIC_KEY_INFO;
struct _CERT_INFO {
    DWORD dwVersion;
    CRYPT_INTEGER_BLOB SerialNumber;
    CRYPT_ALGORITHM_IDENTIFIER SignatureAlgorithm;
    CERT_NAME_BLOB Issuer;
    FILETIME NotBefore;
    FILETIME NotAfter;
    CERT_NAME_BLOB Subject;
    CERT_PUBLIC_KEY_INFO SubjectPublicKeyInfo;
    CRYPT_BIT_BLOB IssuerUniqueId;
    CRYPT_BIT_BLOB SubjectUniqueId;
    DWORD cExtension;
    PCERT_EXTENSION rgExtension;
};
typedef _CERT_INFO* PCERT_INFO;
struct _CERT_CONTEXT {
    DWORD dwCertEncodingType;
    BYTE* pbCertEncoded;
    DWORD cbCertEncoded;
    PCERT_INFO pCertInfo;
    HCERTSTORE hCertStore;
};
typedef _CERT_CONTEXT CERT_CONTEXT;
typedef const CERT_CONTEXT* PCCERT_CONTEXT;
MAICORD_SYS_TYPES_END

// <bcrypt.h>
extern "C" NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* phAlgorithm, LPCWSTR pszAlgId,
    LPCWSTR pszImplementation, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptGetProperty(BCRYPT_HANDLE hObject, LPCWSTR pszProperty, PUCHAR pbOutput,
    ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptGenerateSymmetricKey(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbKeyObject, ULONG cbKeyObject, PUCHAR pbSecret, ULONG cbSecret, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptGenerateKeyPair(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE* phKey,
    ULONG dwLength, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptEncrypt(BCRYPT_KEY_HANDLE hKey, PUCHAR pbInput, ULONG cbInput, void* pPaddingInfo,
    PUCHAR pbIV, ULONG cbIV, PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptDecrypt(BCRYPT_KEY_HANDLE hKey, PUCHAR pbInput, ULONG cbInput, void* pPaddingInfo,
    PUCHAR pbIV, ULONG cbIV, PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptExportKey(BCRYPT_KEY_HANDLE hKey, BCRYPT_KEY_HANDLE hExportKey, LPCWSTR pszBlobType,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptImportKeyPair(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE hImportKey,
    LPCWSTR pszBlobType, BCRYPT_KEY_HANDLE* phKey, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptFinalizeKeyPair(BCRYPT_KEY_HANDLE hKey, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptDestroyKey(BCRYPT_KEY_HANDLE hKey);
extern "C" NTSTATUS WINAPI BCryptDestroySecret(BCRYPT_SECRET_HANDLE hSecret);
extern "C" NTSTATUS WINAPI BCryptSignHash(BCRYPT_KEY_HANDLE hKey, void* pPaddingInfo, PUCHAR pbInput, ULONG cbInput,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptVerifySignature(BCRYPT_KEY_HANDLE hKey, void* pPaddingInfo, PUCHAR pbHash,
    ULONG cbHash, PUCHAR pbSignature, ULONG cbSignature, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptSecretAgreement(BCRYPT_KEY_HANDLE hPrivKey, BCRYPT_KEY_HANDLE hPubKey,
    BCRYPT_SECRET_HANDLE* phAgreedSecret, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptDeriveKey(BCRYPT_SECRET_HANDLE hSharedSecret, LPCWSTR pwszKDF,
    BCryptBufferDesc* pParameterList, PUCHAR pbDerivedKey, ULONG cbDerivedKey, ULONG* pcbResult, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptCreateHash(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_HASH_HANDLE* phHash,
    PUCHAR pbHashObject, ULONG cbHashObject, PUCHAR pbSecret, ULONG cbSecret, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptHashData(BCRYPT_HASH_HANDLE hHash, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptFinishHash(BCRYPT_HASH_HANDLE hHash, PUCHAR pbOutput, ULONG cbOutput, ULONG dwFlags);
extern "C" NTSTATUS WINAPI BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash);
extern "C" NTSTATUS WINAPI BCryptHash(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbSecret, ULONG cbSecret, PUCHAR pbInput,
    ULONG cbInput, PUCHAR pbOutput, ULONG cbOutput);
extern "C" NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer,
    ULONG dwFlags);

// <dpapi.h>
extern "C" BOOL WINAPI CryptProtectData(DATA_BLOB* pDataIn, LPCWSTR szDataDescr, DATA_BLOB* pOptionalEntropy,
    PVOID pvReserved, CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct, DWORD dwFlags, DATA_BLOB* pDataOut);
extern "C" BOOL WINAPI CryptUnprotectData(DATA_BLOB* pDataIn, LPWSTR* ppszDataDescr, DATA_BLOB* pOptionalEntropy,
    PVOID pvReserved, CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct, DWORD dwFlags, DATA_BLOB* pDataOut);
