#pragma once

// Compiler intrinsics, declared as the SDK declares them; see maicord/sys/sdk.hpp.

#if defined(__linux__)
// Linux: the MSVC intrinsics shared code names are clang builtins (with the Windows signatures) and the twin's
// wrappers; __fastfail ends the process at once (the code only means something to Windows' failure report).
extern "C" __declspec(noreturn) void __fastfail(unsigned int);
#include "maicord/os/linux/intrin.hpp"
#else

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/sys/intrin_mem.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <intrin.h>
#endif

extern "C" {

// <intrin0.inl.h>
unsigned char _BitScanForward(unsigned long* _Index, unsigned long _Mask);
unsigned char _BitScanForward64(unsigned long* _Index, unsigned __int64 _Mask);
unsigned char _BitScanReverse(unsigned long* _Index, unsigned long _Mask);
unsigned char _BitScanReverse64(unsigned long* _Index, unsigned __int64 _Mask);
long _InterlockedCompareExchange(long volatile* _Destination, long _Exchange, long _Comparand);
__int64 _InterlockedCompareExchange64(__int64 volatile* _Destination, __int64 _Exchange, __int64 _Comparand);
long _InterlockedDecrement(long volatile* _Addend);
long _InterlockedExchange(long volatile* _Target, long _Value);
__int64 _InterlockedExchange64(__int64 volatile* _Target, __int64 _Value);
long _InterlockedExchangeAdd(long volatile* _Addend, long _Value);
__int64 _InterlockedExchangeAdd64(__int64 volatile* _Addend, __int64 _Value);
long _InterlockedIncrement(long volatile* _Addend);
__int64 _InterlockedIncrement64(__int64 volatile* _Addend);
long _InterlockedOr(long volatile* _Value, long _Mask);
void _ReadWriteBarrier(void);
void _mm_pause(void);
unsigned __int64 __popcnt64(unsigned __int64);

// <intrin.h>
void* _InterlockedCompareExchangePointer(void* volatile* _Destination, void* _Exchange, void* _Comparand);
void* _InterlockedExchangePointer(void* volatile* _Target, void* _Value);
void* _ReturnAddress(void);
void __cpuid(int[4], int);
void __cdecl __debugbreak(void);
__declspec(noreturn) void __fastfail(unsigned int);
unsigned __int64 __rdtsc(void);
unsigned long __readgsdword(unsigned long);
unsigned __int64 __readgsqword(unsigned long);
[[nodiscard]] unsigned __int64 __cdecl _byteswap_uint64(unsigned __int64);
[[nodiscard]] unsigned long __cdecl _byteswap_ulong(unsigned long);
[[nodiscard]] unsigned short __cdecl _byteswap_ushort(unsigned short);

}

#endif // __linux__
