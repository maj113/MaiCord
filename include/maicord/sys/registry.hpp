#pragma once

// Registry (advapi32), declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

// <winreg.h>
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_BINARY 0x00000008
#define RRF_RT_REG_MULTI_SZ 0x00000020
#define RRF_RT_ANY 0x0000ffff
#define HKEY_CURRENT_USER (( HKEY ) (ULONG_PTR)((LONG)0x80000001) )

typedef LONG LSTATUS;
typedef ACCESS_MASK REGSAM;

// <winreg.h>
extern "C" __declspec(dllimport) LSTATUS WINAPI RegCloseKey(HKEY hKey);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved,
    LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired, const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    PHKEY phkResult, LPDWORD lpdwDisposition);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegDeleteKeyW(HKEY hKey, LPCWSTR lpSubKey);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegDeleteValueW(HKEY hKey, LPCWSTR lpValueName);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    REGSAM samDesired, PHKEY phkResult);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegQueryInfoKeyW(HKEY hKey, LPWSTR lpClass, LPDWORD lpcchClass,
    LPDWORD lpReserved, LPDWORD lpcSubKeys, LPDWORD lpcbMaxSubKeyLen, LPDWORD lpcbMaxClassLen, LPDWORD lpcValues,
    LPDWORD lpcbMaxValueNameLen, LPDWORD lpcbMaxValueLen, LPDWORD lpcbSecurityDescriptor,
    PFILETIME lpftLastWriteTime);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved,
    DWORD dwType, const BYTE* lpData, DWORD cbData);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegDeleteKeyValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValueName);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegDeleteTreeW(HKEY hKey, LPCWSTR lpSubKey);
extern "C" __declspec(dllimport) LSTATUS WINAPI RegGetValueW(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue,
    DWORD dwFlags, LPDWORD pdwType, PVOID pvData, LPDWORD pcbData);
