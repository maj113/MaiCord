#pragma once

// SSPI and Schannel, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/crypto.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#define SECURITY_WIN32
#include <Security.h>
#include <Schannel.h>
#endif

// <sspi.h>
#define SecInvalidateHandle( x ) ((PSecHandle) (x))->dwLower = ((PSecHandle) (x))->dwUpper = ((ULONG_PTR) ((INT_PTR)-1)) ;
#define SecIsValidHandle( x ) ( ( ((PSecHandle) (x))->dwLower != ((ULONG_PTR) ((INT_PTR) -1 ))) && ( ((PSecHandle) (x))->dwUpper != ((ULONG_PTR) ((INT_PTR) -1 ))) )
#define SECBUFFER_VERSION 0
#define SECBUFFER_EMPTY 0
#define SECBUFFER_DATA 1
#define SECBUFFER_TOKEN 2
#define SECBUFFER_EXTRA 5
#define SECBUFFER_STREAM_TRAILER 6
#define SECBUFFER_STREAM_HEADER 7
#define SECURITY_NATIVE_DREP 0x00000010
#define SECPKG_CRED_OUTBOUND 0x00000002
#define ISC_REQ_REPLAY_DETECT 0x00000004
#define ISC_REQ_SEQUENCE_DETECT 0x00000008
#define ISC_REQ_CONFIDENTIALITY 0x00000010
#define ISC_REQ_ALLOCATE_MEMORY 0x00000100
#define ISC_REQ_EXTENDED_ERROR 0x00004000
#define ISC_REQ_STREAM 0x00008000
#define SECPKG_ATTR_STREAM_SIZES 4

// <Schannel.h>
#define UNISP_NAME_W L"Microsoft Unified Security Protocol Provider"
#define SCHANNEL_CRED_VERSION 0x00000004
#define SCH_CRED_NO_DEFAULT_CREDS 0x00000010
#define SECPKG_ATTR_CONNECTION_INFO 0x5a
#define SECPKG_ATTR_SESSION_INFO 0x5d
#define SSL_SESSION_RECONNECT 1
#define SCH_CRED_AUTO_CRED_VALIDATION 0x00000020
#define SCH_USE_STRONG_CRYPTO 0x00400000

struct _HMAPPER;
typedef WCHAR SEC_WCHAR;
typedef void(__stdcall* SEC_GET_KEY_FN)(void* Arg, void* Principal, unsigned long KeyVer, void** Key,
    SECURITY_STATUS* Status);

MAICORD_SYS_TYPES_BEGIN
// <sspi.h>
struct _SecHandle {
    ULONG_PTR dwLower;
    ULONG_PTR dwUpper;
};
typedef _SecHandle SecHandle;
typedef _SecHandle* PSecHandle;
typedef SecHandle CredHandle;
typedef PSecHandle PCredHandle;
typedef SecHandle CtxtHandle;
typedef PSecHandle PCtxtHandle;
typedef LARGE_INTEGER SECURITY_INTEGER;
typedef SECURITY_INTEGER TimeStamp;
typedef SECURITY_INTEGER* PTimeStamp;
struct _SecPkgInfoW {
    unsigned long fCapabilities;
    unsigned short wVersion;
    unsigned short wRPCID;
    unsigned long cbMaxToken;
    SEC_WCHAR* Name;
    SEC_WCHAR* Comment;
};
typedef _SecPkgInfoW* PSecPkgInfoW;
struct _SecBuffer {
    unsigned long cbBuffer;
    unsigned long BufferType;
    void* pvBuffer;
};
typedef _SecBuffer SecBuffer;
typedef _SecBuffer* PSecBuffer;
struct _SecBufferDesc {
    unsigned long ulVersion;
    unsigned long cBuffers;
    PSecBuffer pBuffers;
};
typedef _SecBufferDesc SecBufferDesc;
typedef _SecBufferDesc* PSecBufferDesc;
struct _SecPkgContext_StreamSizes {
    unsigned long cbHeader;
    unsigned long cbTrailer;
    unsigned long cbMaximumMessage;
    unsigned long cBuffers;
    unsigned long cbBlockSize;
};
typedef _SecPkgContext_StreamSizes SecPkgContext_StreamSizes;
// <Schannel.h>
struct _SecPkgContext_ConnectionInfo {
    DWORD dwProtocol;
    ALG_ID aiCipher;
    DWORD dwCipherStrength;
    ALG_ID aiHash;
    DWORD dwHashStrength;
    ALG_ID aiExch;
    DWORD dwExchStrength;
};
typedef _SecPkgContext_ConnectionInfo SecPkgContext_ConnectionInfo;
struct _SecPkgContext_SessionInfo {
    DWORD dwFlags;
    DWORD cbSessionId;
    BYTE rgbSessionId[32];
};
typedef _SecPkgContext_SessionInfo SecPkgContext_SessionInfo;
struct _SCHANNEL_CRED {
    DWORD dwVersion;
    DWORD cCreds;
    PCCERT_CONTEXT* paCred;
    HCERTSTORE hRootStore;
    DWORD cMappers;
    struct _HMAPPER** aphMappers;
    DWORD cSupportedAlgs;
    ALG_ID* palgSupportedAlgs;
    DWORD grbitEnabledProtocols;
    DWORD dwMinimumCipherStrength;
    DWORD dwMaximumCipherStrength;
    DWORD dwSessionLifespan;
    DWORD dwFlags;
    DWORD dwCredFormat;
};
typedef _SCHANNEL_CRED SCHANNEL_CRED;
MAICORD_SYS_TYPES_END

// <sspi.h>
extern "C" __declspec(dllimport) SECURITY_STATUS WINAPI AcquireCredentialsHandleW(LPWSTR pszPrincipal,
    LPWSTR pszPackage, unsigned long fCredentialUse, void* pvLogonId, void* pAuthData, SEC_GET_KEY_FN pGetKeyFn,
    void* pvGetKeyArgument, PCredHandle phCredential, PTimeStamp ptsExpiry);
extern "C" __declspec(dllimport) SECURITY_STATUS WINAPI InitializeSecurityContextW(PCredHandle phCredential,
    PCtxtHandle phContext, SEC_WCHAR* pszTargetName, unsigned long fContextReq, unsigned long Reserved1,
    unsigned long TargetDataRep, PSecBufferDesc pInput, unsigned long Reserved2, PCtxtHandle phNewContext,
    PSecBufferDesc pOutput, unsigned long* pfContextAttr, PTimeStamp ptsExpiry);
extern "C" SECURITY_STATUS WINAPI CompleteAuthToken(PCtxtHandle phContext, PSecBufferDesc pToken);
extern "C" __declspec(dllimport) SECURITY_STATUS WINAPI DeleteSecurityContext(PCtxtHandle phContext);
extern "C" __declspec(dllimport) SECURITY_STATUS WINAPI QueryContextAttributesW(PCtxtHandle phContext,
    unsigned long ulAttribute, void* pBuffer);
extern "C" SECURITY_STATUS WINAPI FreeContextBuffer(PVOID pvContextBuffer);
extern "C" SECURITY_STATUS WINAPI EncryptMessage(PCtxtHandle phContext, unsigned long fQOP, PSecBufferDesc pMessage,
    unsigned long MessageSeqNo);
extern "C" SECURITY_STATUS WINAPI DecryptMessage(PCtxtHandle phContext, PSecBufferDesc pMessage,
    unsigned long MessageSeqNo, unsigned long* pfQOP);
extern "C" __declspec(dllimport) SECURITY_STATUS WINAPI QuerySecurityPackageInfoW(LPWSTR pszPackageName,
    PSecPkgInfoW* ppPackageInfo);
